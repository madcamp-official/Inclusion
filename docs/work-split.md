# 작업 분담 — A(모드 1) / B(모드 2)

> 기준 문서: [모드1_모드2_최종사양서.md](./모드1_모드2_최종사양서.md)
> 스택: **JUCE (C++)**, 데스크톱, 저지연 오디오 콜백 기반
> 사양서에서 **"모드 1은 친구분 담당"**이라고 명시되어 있으므로, 아래에서
> - **A = 모드 1(보컬 추종 모드) 담당**
> - **B = 모드 2(기타 보코더 모드) 담당**
> 으로 고정한다.

## 폴더 구조

```
week4/
├── docs/                          # 사양서, 작업 분담 (이 문서)
├── CMakeLists.txt                 # (추가 예정) 빌드 설정 — 최초 1회는 함께 세팅
├── external/
│   └── soundtouch/                # SoundTouch 벤더링 (B 사용)
├── src/
│   ├── main/                      # 앱 진입점, 전체 배선 — 공용, 변경 시 상대방에게 공지
│   ├── core/                      # ★ 공용 인프라 — A/B 모두 사용, 인터페이스는 같이 정의
│   │   ├── audio/                 # 오디오 I/O, 링버퍼, 하울링 캘리브레이션
│   │   ├── dsp/                   # OnsetDetector, PitchDetector, PitchStabilizer(7.5절 상태기계)
│   │   └── song/                  # SongData(코드 진행/가사), SongClock
│   ├── mode1_vocal_follower/      # ★ A 담당 — 모드 1 전용 로직
│   ├── mode2_guitar_vocoder/      # ★ B 담당 — 모드 2 전용 로직
│   ├── ui/
│   │   ├── shared/                # 공용 화면(캘리브레이션, 모드 선택) — 공용
│   │   ├── mode1/                 # ★ A 담당
│   │   └── mode2/                 # ★ B 담당
│   └── params/                    # 10절류 파라미터 표 — 공용, 각자 자기 파트 값만 추가
├── assets/
│   └── songs/<song_id>/           # 코드 차트, 프레이즈 WAV(A), melody 참고 데이터(B, 선택)
├── tests/
│   ├── mode1/                     # A
│   └── mode2/                     # B
└── build/                         # 빌드 산출물 (git 추적 안 함)
```

## A — 모드 1 (보컬 추종 모드) 담당 영역

작업 폴더: `src/mode1_vocal_follower/`, `src/ui/mode1/`, `tests/mode1/`

사양서 PART 2 기준으로 구현할 것:

- **PhraseOnsetDetector** — 프레이즈 시작 판단 (온셋 타이밍 / 코드 전환 검출 / 조합 — 4절에서 확인 필요한 사항)
- **PhraseScheduler** — 예정 프레이즈 시작 시점 vs 실제 연주 타이밍 비교, 허용 범위 판정, 겹침·공백 처리
- **PhrasePlayer** — 프레이즈 WAV 재생, 연주 시작→페이드인(수십ms~150ms), 연주 정지→페이드아웃(200~300ms)
- **SongClock 연동** — 프레이즈가 재정렬되면 화면에 보이는 곡 시계도 같이 움직여야 함 (`core/song/SongClock`은 공용이므로 인터페이스는 B와 상의 후 변경)
- 원본 계획서 그대로 유효한 부분(그대로 가져와 구현):
  - 연주 활동 감지 (8.2절)
  - 보컬 표현 매핑 (8.5절)
  - 실패 대응 (8.6절)
- UI: `src/ui/mode1/Mode1Screen` — 코드·가사 진행 표시

**먼저 확정해야 할 것 (사양서 PART 2 - 4절)**: 프레이즈 시작 판단 기준, 재정렬 허용 범위, 곡 시계 연동 방식. 이건 구현 전에 B/본인과 짧게 합의하고 시작하는 게 좋음.

## B — 모드 2 (기타 보코더 모드) 담당 영역

작업 폴더: `src/mode2_guitar_vocoder/`, `src/ui/mode2/`, `tests/mode2/`, `external/soundtouch/`

사양서 PART 1 기준으로 구현할 것:

- **GuitarTargetTracker** — 기타 온셋→피치 검출→안정화(`core/dsp/PitchStabilizer`의 IDLE→ATTACK→COLLECT→STABLE→COAST 상태기계 사용)→목표 음정 확정, HOLD/FADE 처리
- **CorrectionCalculator** — `compute_correction` (목표-현재 음정 차이, `MAX_CORRECTION_ST` 초과 시 보정 포기)
- **PitchShifterEngine** — SoundTouch 래핑, strength 적용, 목표 전환 시 시프트 램프
- **VocalDelayBuffer** — 목소리 경로 인위적 지연 (온셋 검출 지연 기준 계산)
- **하울링 대책** — 캘리브레이션 시 하울링 임계 측정 → 마스터 게인 상한, 목소리 검출 신뢰도 낮을 때 M2-4 규칙 적용 (`core/audio/FeedbackCalibrator`는 공용이므로 인터페이스는 A와 상의 후 변경)
- UI: `src/ui/mode2/Mode2Screen` — 기타 음정/내 목소리 음정/보정 후 음정/추종도 표시 (S6 화면)

**하지 않는 것 (11절)**: ScoreFollower, BackingRenderer, 반주 오디오 버스/믹서 설정, 기타 모니터 출력, 참조 멜로디 기반 실시간 목표 결정 — 전부 구현 대상 아님.

## 공용 영역 (`src/core/`, `src/ui/shared/`, `src/main/`, `src/params/`)

두 모드가 같이 쓰는 코드다. 각자 담당 파트에서 필요해서 바꿔야 하면:
1. 먼저 상대방에게 어떤 인터페이스가 필요한지 공유
2. 시그니처만 먼저 합의하고 각자 구현체는 따로 작업
3. `OnsetDetector`/`PitchDetector`는 모드 2가 먼저 필요로 하지만(7.5절 상태기계 기반), 모드 1의 프레이즈 온셋 판단에도 재사용 가능성이 있으니 범용으로 설계

## 진행 순서 제안

1. (공동) `CMakeLists.txt` + JUCE 프로젝트 골격, 오디오 디바이스 매니저(`core/audio`) 최소 동작까지 같이 세팅
2. (공동) `core/dsp/OnsetDetector`, `PitchStabilizer` 인터페이스 합의
3. 이후 A/B 각자 담당 폴더에서 병렬 작업
4. 통합은 `src/main/`에서 두 모드를 각각 붙여보며 진행
