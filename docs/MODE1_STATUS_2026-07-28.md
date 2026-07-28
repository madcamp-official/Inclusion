# Mode 1 상태 체크포인트 — 2026-07-28

이 문서는 사용자가 “지금이 딱 좋다”고 확인한 만찬가 Mode 1 상태를
복구하기 위한 기준점이다. 별도의 요청 없이 아래 음역·표현 파라미터를
다시 튜닝하거나 최종 산출물을 덮어쓰지 않는다.

현재 승인 버전은 원곡 표현 슬라이더의 **25% 기본 프리셋**으로
보존한다. 0/25/50/75/100% 중 25%가 앱 시작 기본값이다. 슬라이더는
자동 편한 키(-17)를 변경하지 않고 원곡의 피치·비브라토·에너지 표현
유지량만 변경하며, 선택한 값은 다음 마이크로 구절부터 적용된다.

앱의 수동 키 조절은 `-6`부터 각 곡의 원곡 키에 도달하는 값까지이며
기본값은 `0`이다. 만찬가는 자동 기준 키가 -17이므로 범위가
`-6~+17` semitones이다. 이 값은 보컬과 기타 기대 코드에 동시에
적용된다.

키 앵커는 `-17/-12/-6/0` 네 개이며 각 앵커에는 표현
`0/25/50/75/100`이 있다. 앱은 요청 키와 가장 가까운 사전 렌더
앵커를 선택하고 최대 3반음의 잔여 이동만 SoundTouch로 처리한다.
따라서 `+17`에서는 `0 st` 원곡 키 앵커를 그대로 사용해 실시간
피치 이동이 `0`이다. 현재 선택한 키·표현 WAV 한 개만 메모리에
올리고, 새 뱅크는 다음 구절부터 적용된다. Debug 실측 원곡 키 앵커
전환 시간은 약 `0.66초`였다.

## 사용자 확인 완료 상태

- 곡: 만찬가
- 보컬 엔진: `rvc_user_long.pth`
- 사용자 음성 입력: 로컬 사용자 프로필 WAV(Git 제외)
- 원곡 분리 보컬: 로컬 만찬가 분리 보컬 WAV(Git 제외)
- 자동 기준 키: **-17 semitones**
- 원곡 미세 피치 표현 유지율: **0.18**
- 사용자 비브라토: **4.688 Hz**
- 적용 비브라토 RMS 깊이 상한: **30 cents**
- 고음 강도 감소: 안전 음역 초과 1반음당 **0.8 dB**
- 최대 고음 감소량: **6 dB**
- 크로스페이드: **45 ms**, 듀얼 재생 보이스
- 스트로크 누락 자동 진행 제한: **200 ms**

## 측정 음역

- 사용자 안전 음역: `F2–F3`
- 사용자 중앙 음역: `A#2`
- 기존 RVC -6 핵심 음역: `E3–F4`, 중앙 `A#3`
- 최종 스타일 보정 -17 핵심 음역: `F2–F#3`, 중앙 `C#3`
- 최종 잔여 경고:
  - 안전 저음 아래 약 1.68반음
  - 안전 고음 위 약 0.78반음

## 기준 키 동기화

보컬만 내리는 것이 아니라 패키지의 기대 기타 코드도 -17반음으로
변환한다. 이 규칙을 유지해야 실시간 전조가 중복 적용되지 않는다.

예:

- `Ab/C → Eb/G`
- `Db → Ab`
- `Eb → Bb`
- `Fm7 → Cm7`

## 최종 산출물

- 최종 전체 보컬:
  `build/mode1/bansanka/style_adaptation/bansanka_style_rvc_user.wav`
- 사용자 프로필:
  `build/mode1/bansanka/style_adaptation/voice_profile.json`
- 스타일 계획:
  `build/mode1/bansanka/style_adaptation/style_plan.json`
- 적용 F0:
  `build/mode1/bansanka/style_adaptation/adapted_f0.csv`
- 최종 패키지:
  `build/mode1/bansanka/song_package.json`
- 마이크로 보컬:
  `build/mode1/bansanka/micro_vocals/` (`198`개)
- 비교본:
  `build/mode1/bansanka/style_adaptation/comparison_m6_then_m12_then_auto_m17.wav`
- Mode 1 시뮬레이션:
  `build/mode1/bansanka/simulation/mode1_simulation.wav`
- 실행 파일:
  `build/VocalGuitarApp_artefacts/Debug/Vocal Guitar App.exe`

## 무결성 기준 SHA-256

| 파일 | SHA-256 |
|---|---|
| `bansanka_style_rvc_user.wav` | `CB550701D2D6BD245C5C1CB758CF26ABF82C94DA0413737E9735C358BDBA024D` |
| `voice_profile.json` | `A083B88F19397090C9E82DA02AEE10B12426C2AE84721100BBD57727E2A9AEE6` |
| `style_plan.json` | `871CECBEC0191B7C6F6F923B0EFECCCD83CED8FE317B21C5FDE0D2483BF6E6F0` |
| `song_package.json` | `6BADA88EFD625B4B27C3CA719C98ECCC30739C7E5044AEF631D677CA42C50248` |
| `original-key style 25 WAV` | `9B6485C6EBE71E02DA96F66D86CA8F795398886852364D0936854A28A995680A` |
| `Vocal Guitar App.exe` | `238B5F174DF2B20D5F050F45D522F93295F43EB2D3F39AE91E851FAECD5EF7AC` |

## 재생성 순서

1. `prepare_rvc_guide.py`로 사용자 프로필, 자동 키, WORLD 전처리 생성
2. `run_rvc_remote.py`로 GPU 서버 RVC 실행
3. `build_song_package.py --style-plan-json ...`으로 패키지 재생성
4. `simulate_mode1.py`로 무기타 시뮬레이션 생성
5. 앱과 `Mode1CoreTests` 빌드
6. `ctest --test-dir build -C Debug --output-on-failure`

세부 명령은 `tools/mode1_song_package/README.md`를 따른다.

## 마지막 검증

- Python 도구 문법 검사 통과
- `VocalGuitarApp` Debug 빌드 성공
- `Mode1CoreTests` Debug 빌드 성공
- CTest `2/2` 통과
- 패키지 스키마 버전: `4`
- 구절: `52`
- 마이크로 구절: `198`

## 알려진 한계

현재 처리는 원곡 가수의 피치 흔들림, 비브라토, 강세 잔재를 줄이지만
원곡 자음 타이밍과 호흡 위치는 일부 남는다. 사용자가 현재 결과를
선호하므로 하이브리드 중립 가이드 방식은 기본 파이프라인에 적용하지
않고 이후 별도 실험으로만 진행한다.
