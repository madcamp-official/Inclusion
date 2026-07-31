# Inclusion
몰입캠프 26s-w4-c3-09 프로젝트 repository

기타 연주에 목소리를 얹는 JUCE 앱입니다. 두 가지 모드가 한 앱에 들어 있습니다.

- **모드 1 (보컬 팔로워)** — 기타 스트로크와 코드 변화를 따라 사용자 음색의
  사전 생성 보컬을 재생합니다.
- **모드 2 (기타 보코더)** — 내 목소리를 실시간으로 기타 음정에 맞춰 보정합니다.

## Clone and build

SoundTouch와 WORLD는 `external/`에 소스로 포함되어 있어 추가로 받을 것이 없습니다.

```sh
git clone <repository-url>
cmake -S . -B build
cmake --build build --config Debug --target VocalGuitarApp
```

## Test

테스트는 저작권 있는 곡 파일 없이 실행되도록 합성 fixture를 자동
생성합니다.

```sh
cmake --build build --config Debug --target Mode1CoreTests VoiceCaptureTests Mode2Tests
ctest --test-dir build -C Debug --output-on-failure
```

## 모드 2 오디오 설정

기타와 목소리를 서로 다른 하드웨어에서 동시에 받으려면 통합 기기가 필요합니다.
통합 기기 생성 → 앱 설정 기록 → 실행까지 한 번에:

```sh
tools/mode2-setup.sh airpods    # 또는 speakers, builtin
```

프리셋과 채널 배치, 손으로 만드는 방법은 [오디오 장치 설정](docs/오디오-장치-설정.md)에 있습니다.

모드 2의 WORLD 실시간 하이브리드와 동일 녹음 A/B 방법은
[WORLD / Rubber Band / SoundTouch A/B](docs/pitch-shifter-ab.md)를 참고하세요.

## 목소리 프로필 학습 (Mode 1)

Mode 1은 사용자 본인 목소리로 보컬을 생성하기 위해, 짧은 안내형 녹음
세션으로 음성 프로필을 만들고 그 데이터로 모델을 학습합니다.

- 진행 단계: 말하기 → 모음 지속 발성 → 노래 (3단계, 자동 진행)
- 노래 단계는 최소 2분(120초)까지 이어서 녹음합니다. 그 전에 끝내고
  싶으면 화면의 "지금 제출" 버튼으로 바로 종료할 수 있습니다.
- 세션이 끝나면 화면에서 바로 모델 학습을 시작할 수 있고, 학습에는
  약 8분이 걸립니다.
- 상세 사양은
  [`docs/안내형_음성프로필_생성_사양서.md`](docs/안내형_음성프로필_생성_사양서.md)를
  참고하세요. 최소 녹음 길이 등 일부 수치는 실제 구현과 다를 수 있으니
  최종 기준은 `src/voice_capture/GuidedRecordingSession.h`를 따릅니다.

## Local-only data

사용자 녹음, 분리된 원곡 보컬, RVC/Seed-VC 모델, Python 가상환경,
생성된 WAV와 `build/` 산출물은 Git에 포함하지 않습니다. 곡 패키지
생성 절차는
[`tools/mode1_song_package/README.md`](tools/mode1_song_package/README.md),
공유해야 하는 파일 목록은
[`assets/songs/README.md`](assets/songs/README.md),
현재 승인된 Mode 1 파라미터는
[`docs/MODE1_STATUS_2026-07-28.md`](docs/MODE1_STATUS_2026-07-28.md)를
참고하세요.
