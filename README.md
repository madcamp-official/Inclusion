# Inclusion
몰입캠프 26s-w4-c3-09 프로젝트 repository

기타 스트로크와 코드 변화를 따라 사용자 음색의 사전 생성 보컬을
재생하는 JUCE 기반 Mode 1 프로토타입입니다.

## Clone and build

SoundTouch는 submodule로 고정되어 있습니다.

```powershell
git clone --recurse-submodules <repository-url>
cmake -S . -B build
cmake --build build --config Debug --target VocalGuitarApp
```

이미 clone했다면 다음 명령으로 SoundTouch를 받습니다.

```powershell
git submodule update --init --recursive
```

## Test

테스트는 저작권 있는 곡 파일 없이 실행되도록 합성 fixture를 자동
생성합니다.

```powershell
cmake --build build --config Debug --target Mode1CoreTests
ctest --test-dir build -C Debug --output-on-failure
```

## Local-only data

사용자 녹음, 분리된 원곡 보컬, RVC/Seed-VC 모델, Python 가상환경,
생성된 WAV와 `build/` 산출물은 Git에 포함하지 않습니다. 곡 패키지
생성 절차는
[`tools/mode1_song_package/README.md`](tools/mode1_song_package/README.md),
현재 승인된 Mode 1 파라미터는
[`docs/MODE1_STATUS_2026-07-28.md`](docs/MODE1_STATUS_2026-07-28.md)를
참고하세요.
