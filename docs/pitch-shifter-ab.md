# WORLD / Rubber Band / SoundTouch A/B

모드 2 앱은 Rubber Band를 기본 피치 시프터로 사용한다. WORLD 하이브리드는 음색 보존
가능성을 비교하기 위한 실험 백엔드다. WORLD는 160ms 창을 80ms hop으로 백그라운드
분석하고, 유성음은 기타의 절대 목표 F0로 재합성하며 무성음·호흡은 원본을 통과시킨다.
인접 합성 구간은 40ms equal-power crossfade로 연결한다. WORLD 자체 기준 지연은 약
140ms이며 실제 장치 왕복 지연과 분석 연산 시간이 추가된다. 화면의 보정 후 항목에서
현재 백엔드와 기준 지연을 확인한다.

Rubber Band 설정은 실시간 동적 보컬 피치 시프트를 위한 다음 조합이다.

- `OptionProcessRealTime`
- `OptionEngineFiner` (R3)
- `OptionWindowShort`
- `OptionFormantPreserved`
- `OptionPitchHighConsistency`

Rubber Band를 찾지 못하면 기존 SoundTouch로 자동 폴백한다. macOS에서는 다음과 같이
개발 패키지를 설치할 수 있다.

```sh
brew install rubberband
```

Linux 배포판에서는 `librubberband-dev`처럼 헤더와 라이브러리가 포함된 개발 패키지가
필요하다. 설치 후 CMake를 다시 구성하면 로그에 `Rubber Band found`가 표시된다.

## 같은 녹음으로 비교

앱의 진단 녹음은 3채널 WAV(기타, 목소리, 당시 출력)를 만든다. 같은 입력 파일을 세
백엔드로 각각 처리한다.

```sh
build/Mode2Offline_artefacts/Debug/Mode2Offline input.wav rubberband.wav --shifter=rubberband
build/Mode2Offline_artefacts/Debug/Mode2Offline input.wav soundtouch.wav --shifter=soundtouch
build/Mode2Offline_artefacts/Debug/Mode2Offline input.wav world.wav --shifter=world
```

두 명령의 다른 옵션(`--glide`, `--boost`, `--octave`, `--gate`, `--bleed`, `--block`)은
동일하게 맞춘다. 출력 로그의 `시프터=` 값으로 요청한 백엔드가 실제 사용됐는지 확인할
수 있다.

평가할 때는 특히 큰 하향 시프트에서 모음의 화자 정체성, 자음의 선명도, 음 전환의
끊김, 전체 지연을 비교한다.

이 경로는 전체 녹음을 Harvest + CheapTrick + D4C로 분석하고, 현재 컨트롤러가 만든
보정량만 F0에 적용한다. 유성음은 WORLD로 재합성하고 무성음·호흡은 원본을 짧게
크로스페이드해 통과시킨다. 오프라인 결과는 앱의 짧은 스트리밍 창보다 문맥이 길어서
WORLD 방식 자체의 음질 상한을 확인하는 기준선으로 사용한다.

## 라이선스

Rubber Band 오픈소스판은 GPL로 배포된다. 비공개 상용 배포에는 별도 상용 라이선스가
필요하므로 제품 배포 전에 반드시 라이선스 방식을 확정해야 한다. SoundTouch 백엔드는
A/B와 폴백을 위해 현재 유지한다. Vendored WORLD 코드는 수정 BSD 라이선스다.
