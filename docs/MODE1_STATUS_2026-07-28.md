# Mode 1 상태 체크포인트 — 2026-07-28

이 문서는 사용자가 “지금이 딱 좋다”고 확인한 만찬가 Mode 1 상태를
복구하기 위한 기준점이다. 별도의 요청 없이 아래 음역·표현 파라미터를
다시 튜닝하거나 최종 산출물을 덮어쓰지 않는다.

> **갱신 이력 (2026-07-28 이후)**
> 음역·표현 파라미터는 승인 상태 그대로다. 이후 두 가지가 바뀌었다.
> 1. 마이크로 구절을 `198` → `558`개(모라 단위)로 재분할했다.
> 2. 재생 페이드를 수정했다(등파워 크로스페이드, 항상 적용되는 35 ms
>    끝 페이드, quickseek 해제).
>
> 두 변경 모두 **사용자 청취 승인을 아직 받지 않았다.** 이전 상태는
> `build/mode1/bansanka/_backup_pre_mora/`에서 되돌릴 수 있다.

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
올리고, 새 뱅크는 다음 구절부터 적용된다.

이전에 기록된 Debug 실측 앵커 전환 시간 `0.66초`는 마이크로 구절이
`198`개이던 시점의 값이다. 현재는 `558`개로 재분할되어 이 수치를
다시 재지 않았으므로 유효한 기준으로 쓰지 않는다.

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
- 크로스페이드: **45 ms**, 듀얼 재생 보이스, **등파워(sin) 램프**
- 구절 끝 페이드: **35 ms**, 항상 적용
- SoundTouch quickseek: **off**
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
- 구절 보컬:
  `build/mode1/bansanka/vocals/` (`52`개)
- 마이크로 보컬:
  `build/mode1/bansanka/micro_vocals/` (`558`개, 모라 단위)
- 한글 발음 표기(로컬 전용, Git 제외):
  `build/mode1/bansanka/readings_ko.txt` → 패키지의 `lyrics_reading_ko`
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
| `song_package.json` | `B502EDDE3AA8E5F6684C8EA6CB60056C830767DAB540595F3ED4D0F39A19AA04` |
| `original-key style 25 WAV` | `9B6485C6EBE71E02DA96F66D86CA8F795398886852364D0936854A28A995680A` |
| `Vocal Guitar App.exe` | `C33B2F26752CDEFD1C0DDEF231DC5F5B93B353BFB1E91BA0A1C6525DE44E2EBC` |

`song_package.json`과 실행 파일 해시는 모라 단위 재분할과 재생 페이드
수정으로 갱신됐다. 상위 RVC 산출물(전체 보컬, 프로필, 스타일 계획,
원곡 키 표현 25 WAV)은 재변환하지 않았으므로 값이 그대로다.

## 재생성 순서

1. `prepare_rvc_guide.py`로 사용자 프로필, 자동 키, WORLD 전처리 생성
2. `run_rvc_variants_remote.py`로 GPU 서버 RVC 실행
3. `resample_manifest_audio.py --sample-rate 48000`으로 48 kHz 변환
4. `build_song_package.py --style-plan-json ... --mora-granularity`로
   패키지 재생성
5. `add_style_variants.py`로 기본 키 표현 변형 부착
6. `add_key_anchor.py`를 앵커마다 실행. **기본 키(`-17`) manifest에도
   반드시 한 번 실행해야** `vocal_key_variants`에 `-17`이 들어간다.
   이 단계를 빠뜨리면 앵커가 3개만 붙는다
7. `apply_lyrics_reading.py`로 한글 발음 재적용 (패키지를 다시 만들면
   `lyrics_reading_ko`가 사라지므로 매번 필요)
8. `simulate_mode1.py`로 무기타 시뮬레이션 생성
9. 앱과 테스트 빌드
10. `ctest --test-dir build -C Debug --output-on-failure`

세부 명령은 `tools/mode1_song_package/README.md`를 따른다.

패키지를 재생성하기 전에 기존 `song_package.json`을 백업한다. 현재
모라 재분할 직전 상태는 `build/mode1/bansanka/_backup_pre_mora/`에
보관되어 있다.

## 마지막 검증

- Python 도구 문법 검사 통과
- `VocalGuitarApp` Debug 빌드 성공
- `Mode1CoreTests` / `VoiceCaptureTests` Debug 빌드 성공
- CTest `2/2` 통과
- 패키지 스키마 버전: `4`
- 구절: `52`
- 마이크로 구절: `558` (모라 단위, 중앙값 `0.262초`)
- 키 앵커: `-17/-12/-6/0` × 표현 `0/25/50/75/100` (558개 전부 균일)
- 한글 발음 표기: `42/52` 구절

### 미검증 항목

아래는 코드와 도구는 갖췄지만 실제로 한 바퀴 돌려본 적이 없다.
"동작한다"고 기록하지 않는다.

- 모라 단위 재분할 후의 실기타 청취 확인
- 가이드 녹음 세션 실사용(마이크로 전 단계 완주)
- 녹음 → RVC 재학습 → 새 모델로 곡 변환 전체 사이클

## 알려진 한계

현재 처리는 원곡 가수의 피치 흔들림, 비브라토, 강세 잔재를 줄이지만
원곡 자음 타이밍과 호흡 위치는 일부 남는다. 사용자가 현재 결과를
선호하므로 하이브리드 중립 가이드 방식은 기본 파이프라인에 적용하지
않고 이후 별도 실험으로만 진행한다.
