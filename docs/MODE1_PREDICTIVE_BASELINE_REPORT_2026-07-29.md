# Mode 1 예측형 전환 기준선 보고서

- 동결 시각: 2026-07-29
- Git branch: `feature/guided-recording-and-training`
- Git HEAD: `b5eeadf7995ac4f3a780f7bd0961f661ac7b530b`
- tracked working-tree binary diff blob: `512ae8edeb1c81f8b7788f0658e33ce89cff8281`
- 상태: 사용자 기존 변경을 보존한 비커밋 기준선

## 1. 기준선 원칙

이 보고서는 예측형 엔진 구현 전 상태를 재현하기 위한 기준이다. 기존 수정 파일은 reset, checkout, stash 또는 자동 commit하지 않았다. 이후 구현은 이 상태와 정량 비교하며, baseline scheduler를 feature flag 뒤에 유지한다.

## 2. Git 작업 트리

### Modified

- `src/main/MainComponent.cpp`
- `src/main/MainComponent.h`
- `src/mode1_vocal_follower/Mode1Controller.cpp`
- `src/mode1_vocal_follower/Mode1Controller.h`
- `src/mode1_vocal_follower/PhraseScheduler.cpp`
- `tools/mode1_song_package/build_song_package.py`
- `tools/mode1_song_package/chord_cleanup.py`
- `tools/mode1_song_package/generate_guitar_test_performance.py`
- `tools/mode1_song_package/refresh_song_package_after_training.py`

### Untracked

- `docs/MODE1_PREDICTIVE_SCORE_FOLLOWER_MASTER_PLAN_2026-07-29.md`
- `tools/mode1_song_package/gp5_to_chords.py`
- `tools/mode1_song_package/musicxml_to_mode1_score.py`
- `tools/mode1_song_package/prepend_intro_from_package.py`
- `tools/mode1_song_package/promote_nested_package.py`

## 3. 소스 SHA-256

| 파일 | SHA-256 |
|---|---|
| `src/main/MainComponent.cpp` | `dec59bbc22cf4b6da9ea324de5d168d87b64759f8f76c975a2cfc2359bf21a4a` |
| `src/main/MainComponent.h` | `4e38da8314440d9f87b85d4e3759c47edce21508eda283d3875d5e3d2cf667aa` |
| `src/mode1_vocal_follower/Mode1Controller.cpp` | `2ecd041bdfecd166d194d0391031d438727dbf003d09fa0d08c94c6dc43f5605` |
| `src/mode1_vocal_follower/Mode1Controller.h` | `9e7824b412971e5bdc4f846194a3c0601467b155b34ddc6b0760ac59b35c581b` |
| `src/mode1_vocal_follower/PhraseScheduler.cpp` | `316fdaaba1b72bd33f9b2e0135199252de8a28787d166074f3a3f0dd6f16532e` |
| `tools/mode1_song_package/build_song_package.py` | `f4d6e5f2d87eeaaf85c1396dabdd64b54506fa7d04e3d9ce0c72040016b7d9ff` |
| `tools/mode1_song_package/chord_cleanup.py` | `2b10e73a639f1ca38660b2de0f8ee88280663ebcc0da6f88f28c695126139339` |
| `tools/mode1_song_package/generate_guitar_test_performance.py` | `2596a4adb119a4ccde09d4a4025b47f691e70c24f5fb24300b93d2a9d91f71f0` |
| `tools/mode1_song_package/refresh_song_package_after_training.py` | `4af6972250a70f70bf0083a8a496d873be154cf764b3448a2ee811b01364dbcb` |
| `tools/mode1_song_package/gp5_to_chords.py` | `fda0f2dc4ad8647b54197d0e93b29ac3e4f6fff2fe5cc8a314a42e91bf8e6c72` |
| `tools/mode1_song_package/musicxml_to_mode1_score.py` | `a8170f48d60a360ac9743ab347c4b9c196746ca525a5d7424a2516863a4e03a5` |
| `tools/mode1_song_package/prepend_intro_from_package.py` | `63461fa12ef7cf30f2986a40717cdb699c535b4ea25a39c22c5ae15e9cf2320c` |
| `tools/mode1_song_package/promote_nested_package.py` | `8f5173ff2440565acbe2302158c12c7de9dbe0da9557b4a3257634d6818d1d11` |

## 4. 패키지 기준선

| 곡 | chord events | phrases | micro phrases | 첫 chord | 첫 vocal | 첫 vocal 이전 chord |
|---|---:|---:|---:|---:|---:|---:|
| 만찬가 | 158 | 52 | 149 | 0.000 s | 9.952 s | 9 |
| Don't Look Back in Anger | 210 | 50 | 210 | 11.184 s | 11.184 s | 0 |

### Package SHA-256

- 만찬가: `41206ad74203a4ec042777409d44fe60635b4bcdfd57a8d10fa6c945f13a84d3`
- Don't Look Back in Anger: `dfc38b9f4891d4c6ca82d90f7a6d6ab406949908c8e77e8be693efc41ccaac02`

## 5. Oasis 인트로 결함 기준선

원천 `build/mode1/dont_look_back_in_anger/chords.json`과 최종 package를 비교했다.

| 항목 | 값 |
|---|---:|
| 원천 첫 chord | 0.000000 s |
| 원천의 첫 vocal 이전 chord 수 | 9 |
| 원천 마지막 intro chord | 10.741890 s |
| 최종 package 첫 chord | 11.184000 s |
| 최종 package 첫 vocal | 11.184000 s |
| 최종 package 첫 vocal 이전 chord 수 | 0 |

잠정 원인: 패키지 생성 또는 promote/refresh 과정에서 첫 vocal 이전 chord timeline이 제거됐다. 런타임은 첫 매칭 chord의 `startSeconds`로 song time을 이동시키므로, 첫 onset 직후 첫 vocal이 due가 될 수 있다.

## 6. Fixture 및 실행 파일 SHA-256

- 이전 만찬가 실제 녹음  
  `output/audio/guitar_tests/bansanka_previous_guitar_take_recovered.wav`  
  `e199685bc41d7cf42616a27d6dc59ecb08e568e7df493ab20429653bf82b18f0`
- 현재 소스 재빌드 Mode1CoreTests  
  `ab18237976eb275eb588ce84267b225fff6a678fe5a31bb95d1e414e4a2d79d7`
- 현재 소스 재빌드 Mode1OfflineRenderer  
  `51bd8982a8bf9ec9b270eaa15f4879eb3a3c63c39c8271485bddaca9b6437b0b`

## 7. 테스트 기준선

### 재빌드 전 기존 Release 바이너리

`Mode1CoreTests`에서 7개 assertion 실패:

- early repeated strum
- first vocal chord start
- missing onset auto-advance
- phrase offset within chord
- half-beat subdivision advance
- subdivision early vocal
- extra subdivision cursor advance

`VoiceCaptureTests`는 통과했다.

이 결과는 현재 소스와 기존 바이너리의 불일치 가능성이 있으므로 제품 기준선으로 사용하지 않는다.

### 현재 작업 트리 재빌드 후

```text
Mode1CoreTests: passed
VoiceCaptureTests: passed
Total: 2/2 passed
```

Mode1CoreTests와 Mode1OfflineRenderer는 현재 소스로 정상 재빌드됐다. GUI 앱 링크는 실행 파일을 다른 프로세스가 점유한 상태에서 `LNK1104`가 발생했으므로, 소스 컴파일 실패가 아니라 별도 환경 항목으로 기록한다.

## 8. 알려진 현재 동작

- phrase audio는 package load 시 메모리에 준비된다.
- phrase start 요청은 onset과 같은 audio callback에서 처리된다.
- 현재 scheduler는 다음 인쇄 chord 경계를 strike 전까지 넘지 않는다.
- timing gate 제거 실험은 실제 연주에서 과도한 전진을 만들었다.
- 만찬가 package에는 intro event가 보존되어 있다.
- Oasis 최종 package에는 intro event가 보존되어 있지 않다.

## 9. Phase 0 종료 조건

- [x] Git HEAD, branch, working-tree diff identity 기록
- [x] 기존 변경 및 미추적 파일 목록 기록
- [x] 핵심 소스 SHA-256 기록
- [x] 두 곡 package SHA-256 및 event 통계 기록
- [x] Oasis intro 결함 수치 재현
- [x] 현재 소스 기준 Release 테스트 2/2 통과
- [x] baseline scheduler를 삭제하거나 덮어쓰지 않음

Phase 1부터 모든 새 측정은 이 보고서의 package와 fixture hash를 함께 기록한다.
