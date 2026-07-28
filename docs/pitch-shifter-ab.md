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

## 출력 음정 정확도

음색과 지연은 귀로 비교해야 하지만, "목표 음정에 실제로 맞았는가"는 숫자로 나온다.
`Mode2Offline`은 만들어진 출력을 다시 F0 분석(Harvest + StoneMask)해 그 순간의 기타
목표와 cents 단위로 비교한다.

```
출력 음정 정확도 (출력을 다시 F0 분석해 기타 목표와 비교):
   표본 2392프레임 (유성 2396 / 전체 2401)  정렬 지연 실측 11.7ms (이론 26.7ms)
   ±50 cents 이내 96.9%   ±20 cents 이내 61.1%
   |오차| 중앙값 17.4 cents  상위10% 27.0 cents
   계통 오차(부호 있는 중앙값) -0.6 cents   옥타브 오류(|오차|>600c) 0.2%
```

- **±50 cents 이내**가 주 지표다. 반음의 절반을 넘으면 다른 음으로 들리기 시작한다.
- **계통 오차**가 0에서 멀면 전체가 일정하게 높거나 낮게 나간다는 뜻이라 파라미터로
  교정할 수 있다. 반대로 중앙값은 큰데 계통 오차가 0에 가까우면 목표 주변에서
  흔들린다는 뜻이므로 추종 루프 문제다.
- **정렬 지연**은 출력이 목표 궤적과 가장 잘 맞는 지연을 탐색한 실측치다. 이론값은
  시프터 고유 지연이며 상한에 해당한다(`PitchShifterEngine::reset()`이 Rubber Band의
  start delay만큼을 미리 버려 보상하므로 실측은 더 짧게 나온다). 실측이 이론값을
  넘어서면 계산에 없는 지연이 붙은 것이다.
- 같은 음을 오래 끄는 녹음에서는 지연을 어긋내도 결과가 거의 같아 **식별 불가**로
  표시되고 이론값을 쓴다. 음이 자주 바뀌는 녹음이어야 지연이 실측된다.

### 합성 신호 기준선 (2026-07-27)

12초, 0.25초마다 음이 바뀌는 기타 + 비브라토(±20 cents)가 있는 목소리, `--octave=0`:

| 백엔드 | ±50c | ±20c | 중앙값 | 상위10% | 정렬 지연 |
|---|---|---|---|---|---|
| Rubber Band | 97.1% | 95.2% | 8.4c | 15.5c | 11.7ms |
| SoundTouch | 89.8% | 88.2% | 5.6c | 53.8c | 33.7ms |
| WORLD(오프라인) | 98.5% | 97.1% | 1.5c | 5.9c | 0ms |

SoundTouch는 정상 상태 중앙값은 좋은데 상위10%가 크다 — 음이 바뀌는 순간에 크게
벗어난다는 뜻이다. Rubber Band는 반대로 고르게 맞는다.

### 오차는 시프터가 아니라 제어 루프에서 나온다

같은 신호에서 목소리의 비브라토와 드리프트만 없애면 Rubber Band의 오차 중앙값이
17.4 → **0.1 cents**가 된다(정렬 21.3ms 시절 측정). 즉 시프터 자체는 사실상 완벽하고,
오차는 흔들리는 목소리를 따라가 상쇄하지 못하는 제어 루프에서 나온다.

출력 음정 = 목표 + (실제 보컬 − 추정 보컬)이므로, **보컬 추정의 오차와 지연이 그대로
출력 음정 오차가 된다**. 이 구조적 성질 때문에 "측정해서 빼는" 설계에는 추종 오차가
원리적으로 남는다. WORLD가 비브라토가 있어도 1.5 cents인 이유가 여기 있다 — 보컬 F0를
상쇄하는 대신 기타의 절대 F0로 직접 합성하므로 추종 오차라는 개념 자체가 없다.
Rubber Band는 피치 배율만 받으므로 이 성질을 얻을 수 없다.

이 지표로 `vocalControlLookaheadSeconds`를 21.3ms → 0으로 바꿨다(중앙값 17.3 → 8.4).
근거는 `src/params/Mode2Params.h`의 해당 주석에 표로 남겼다.

이 숫자들은 합성 신호 기준이라 실제 녹음의 절대값과는 다르다. 백엔드·파라미터를
바꿨을 때의 **상대 비교**에 쓴다.

### 음역별 기준선 (2026-07-28)

"고음으로 부르면 출력이 따라 올라간다"처럼 **음역에 따라 달라지는** 문제는 위 표로
안 잡힌다. 한 녹음의 음역이 하나뿐이기 때문이다. `tools/octave_probe.py`는 같은 기타
반주에 목소리 음역만 바꾼 파일들을 만든다.

```sh
python3 tools/octave_probe.py /tmp/probe
for f in /tmp/probe/vocal_*.wav; do
    build/Mode2Offline_artefacts/Debug/Mode2Offline "$f" /tmp/out.wav \
        --octave=0 --gate=off --bleed=off | grep -E "입력 |±50|옥타브 오류"
done
```

Rubber Band, `--octave=0`, 목소리 탐색 상한 450Hz → 900Hz로 넓히기 전후:

| 목소리 | ±50c (450→900) | 옥타브 오류 (450→900) |
|---|---|---|
| A2 (110~131Hz) | 46.6% → 46.6% | 49.5% → 49.5% (아래 주의 참고) |
| D3~E4 | 92~96% → 변화 없음 | 0.0% → 0.0% |
| A4 (440~523Hz) | 21.7% → **89.1%** | 76.0% → **0.5%** |
| C5 (523~622Hz) | 0.0% → **88.0%** | 100.0% → **0.0%** |
| E5 (659~784Hz) | 0.0% → **83.4%** | 100.0% → **1.1%** |

근거와 상한 선택 과정은 `src/params/Mode2Params.h`의 `vocalPitchMaxFrequencyHz` 주석에
표로 남겼다.

**주의 — A2 칸은 지표의 착시다.** 이 조합에서 출력은 배음이 거의 없는 사인파에 가까워지고
(큰 상향 시프트 + 포먼트 보존), Harvest가 절반쯤의 프레임에서 배주기를 잡는다. 같은 출력을
스펙트럼 최대 성분으로 재보면 ±50c 94%, 옥타브 오류 1%로 정상이다. 이 지표는 사람 목소리
같은 배음 구조를 전제하므로, **합성 신호에서 한 칸만 유독 나쁘면 먼저 출력 스펙트럼을 직접
보고 진짜인지 확인할 것.** 실제 녹음에서는 나타나지 않았다.

이 경로는 전체 녹음을 Harvest + CheapTrick + D4C로 분석하고, 현재 컨트롤러가 만든
보정량만 F0에 적용한다. 유성음은 WORLD로 재합성하고 무성음·호흡은 원본을 짧게
크로스페이드해 통과시킨다. 오프라인 결과는 앱의 짧은 스트리밍 창보다 문맥이 길어서
WORLD 방식 자체의 음질 상한을 확인하는 기준선으로 사용한다.

## 라이선스

Rubber Band 오픈소스판은 GPL로 배포된다. 비공개 상용 배포에는 별도 상용 라이선스가
필요하므로 제품 배포 전에 반드시 라이선스 방식을 확정해야 한다. SoundTouch 백엔드는
A/B와 폴백을 위해 현재 유지한다. Vendored WORLD 코드는 수정 BSD 라이선스다.
