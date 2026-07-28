# [컨텍스트 문서] JUCE 기반 실시간 오디오 앱의 입력 데이터 형식과 처리 경로 (Windows 기준 / macOS 차이 포함)

> **이 문서를 읽는 AI에게**: 아래는 실제로 동작 중인 C++/JUCE 오디오 앱의 입력 경로를 코드에서
> 그대로 추출한 사실 기록입니다. 외부 파일을 열어볼 필요가 없도록 근거 코드를 본문에 포함했습니다.
> 이 문서에 없는 수치나 동작은 추측하지 말고 "문서에 없음"이라고 답해 주세요.
>
> **개발 환경 주의**: 원 개발은 **macOS**에서 이루어졌고, 이 문서를 쓰는 시점에 **Windows 빌드는
> 검증되지 않았습니다.** 아래 9절(Windows ↔ macOS 차이)의 항목 중 "코드 확인됨"으로 표시한 것은
> 소스에서 직접 확인한 사실이고, "플랫폼 일반"으로 표시한 것은 Windows/JUCE의 일반적 동작입니다.

---

## 0. 프로젝트 전제

- **언어/프레임워크**: C++20, **JUCE 8.0.15** (CMake `FetchContent`로 자동 내려받음)
- **하는 일**: 사용자가 기타로 단음 멜로디를 치면서 동시에 노래하면, **기타가 낸 음을 목표 음정으로
  삼아 마이크 목소리를 실시간으로 그 음정에 맞춰 변조**해 내보낸다.
- **중요**: 기타는 소리를 내는 악기가 아니라 **음정 컨트롤러**다. 스피커로 나가는 소리는
  변조된 목소리 하나뿐이고, 기타 소리는 출력에 절대 섞이지 않는다.
- 핵심 클래스: `MainComponent`(JUCE `AudioAppComponent` 상속, 장치·콜백 담당) →
  `Mode2Controller`(DSP 파이프라인 전체) → `GuitarTargetTracker` / `PitchDetector` / `PitchShifterEngine`

```
[기타]  ──1/4" 라인·DI──┐                   ┌─ Windows: WASAPI / DirectSound (ASIO는 선택)
                        ├─→ 오디오 인터페이스 ┤                                    ─→ JUCE AudioDeviceManager
[마이크] ──XLR/USB/BT──┘   (A/D 변환)       └─ macOS: CoreAudio                              │
                                                                                             ▼
                                                   getNextAudioBlock(AudioSourceChannelInfo)
                                                       float, 채널별 비인터리브, -1.0 ~ +1.0
                                                                                             │
                                         ┌───────────────────────────────────────────────────┴──────────┐
                                         ▼                                                              ▼
                                ch[기타 인덱스]                                                 ch[목소리 인덱스]
                          온셋 → 피치 → 안정화 = 목표 음정                        유입상쇄 → 부스트 → 피치검출 → 피치시프트
                                         └──────────────────→ 보정량 계산 ←───────────────────┘
                                                                 │
                                                                 ▼
                                                     outL / outR (동일한 모노 신호)
                                                                 │
                                              ┌──────────────────┴──────────────────┐
                                              ▼                                     ▼
                                       D/A → 스피커/헤드폰               3채널 진단 WAV(녹음 켠 경우)
```

**OS별로 다른 것은 위 그림의 맨 왼쪽 한 칸(드라이버 계층)뿐이다.** JUCE 콜백에 도달한 뒤의
데이터 형식과 DSP 경로는 Windows와 macOS가 완전히 동일하다.

---

## 1. 장치를 여는 코드와 채널 수

```cpp
// MainComponent 생성자
std::unique_ptr<juce::XmlElement> savedAudioState;
if (const auto settingsFile = getAudioSettingsFile(); settingsFile.existsAsFile())
    savedAudioState = juce::XmlDocument::parse(settingsFile);

setAudioChannels(8, 2, savedAudioState.get());   // 입력 8 요청, 출력 2
```

**사실 목록**

- 입력 **8채널을 여유 있게 요청**한다. 실제로 열리는 채널 수는 장치가 정한다(2in 인터페이스면 2채널).
  - macOS에서 8을 요청한 원래 이유는 **통합 기기(Aggregate Device)** 다. 오디오 인터페이스와 내장
    마이크를 하나로 묶으면 입력 채널이 3개 이상이 되기 때문.
  - **Windows에는 이 개념이 없다**(→ 9절 D-3). 따라서 실제 채널 수는 인터페이스가 제공하는 만큼이다.
    요청값 8은 그냥 상한이므로 Windows에서도 그대로 두면 된다.
- 출력은 **2채널**이지만 내용물은 좌·우가 완전히 같은 모노다.
- 앱 내 장치 설정 다이얼로그: `juce::AudioDeviceSelectorComponent(deviceManager, 1, 8, 1, 2, false, false, false, false)`
  → 입력 1~8, 출력 1~2 범위에서 선택 가능. **Windows에서는 이 다이얼로그에 드라이버 종류
  (WASAPI / DirectSound / ASIO) 선택 콤보가 하나 더 나타난다.**
- 장치 선택 상태는 XML로 저장/복원한다(`deviceManager.createStateXml()`).
  - **Windows**: `%APPDATA%\VocalGuitarApp\AudioDeviceSettings.xml`
  - macOS: `~/Library/VocalGuitarApp/AudioDeviceSettings.xml`
- **샘플레이트와 블록 크기는 앱이 정하지 않는다.** 장치가 정하고, 앱은 아래 콜백으로 통보받는다:

```cpp
void MainComponent::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    currentSampleRate = sampleRate;
    mode2Controller.prepare(sampleRate, samplesPerBlockExpected);
    guitarInputScratch.assign((size_t) samplesPerBlockExpected, 0.0f);
    vocalInputScratch .assign((size_t) samplesPerBlockExpected, 0.0f);
}
```

> **Windows 추가 제약**: WASAPI **공유 모드**에서는 샘플레이트를 앱이 요청할 수 없고, 제어판
> (`mmsys.cpl` → 장치 → 고급 → 기본 형식)에 설정된 값이 그대로 내려온다. 원하는 레이트로 열려면
> **독점 모드(Exclusive)** 나 ASIO를 써야 한다. macOS는 CoreAudio가 앱의 레이트 요청을 받아준다.

---

## 2. 콜백으로 들어오는 데이터의 정확한 형식

**이 표는 Windows와 macOS가 동일하다.** JUCE가 드라이버 차이를 흡수한다.

| 항목 | 값 |
|---|---|
| 자료형 | `float` (32비트 부동소수) |
| 값의 범위 | `-1.0 ~ +1.0` (0dBFS = 1.0). 인터페이스의 24비트 정수 샘플이 이미 정규화되어 들어옴 |
| 메모리 배치 | **비인터리브(non-interleaved)** — 채널마다 별도의 연속 배열. `buffer.getReadPointer(ch, startSample)` |
| 한 블록 길이 | `bufferToFill.numSamples` (= 장치 버퍼 크기, 보통 128 / 256 / 512) |
| 시작 오프셋 | `bufferToFill.startSample` — 0이 아닐 수 있으므로 항상 더해서 읽어야 함 |
| 샘플레이트 | 장치가 결정. 44.1kHz / 48kHz가 보통 (블루투스 마이크는 훨씬 낮아질 수 있음 → 4절) |
| 채널 순서 | 인터페이스의 물리 입력 순서. 예) Scarlett Solo = INPUT 1 XLR 마이크, INPUT 2 기타 잭 |

### 2-1. 이 API의 가장 큰 함정: 입력 버퍼 == 출력 버퍼

JUCE의 `getNextAudioBlock`은 **같은 버퍼에 입력이 담겨 오고, 거기에 출력을 덮어써서 돌려주는** 구조다.
따라서 관용적으로 맨 앞에서 `clearActiveBufferRegion()`을 호출하면 **입력을 읽기도 전에 지워버린다.**

```cpp
void MainComponent::getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill)
{
    // 주의: 이 버퍼에는 입력 샘플이 담겨 온다. clearActiveBufferRegion()을 먼저 부르면
    // 입력을 읽기 전에 지워버리므로, 처리하지 않는 경우에만 비운다.
    if (!mode2Active || bufferToFill.buffer->getNumChannels() < 2)
    {
        bufferToFill.clearActiveBufferRegion();
        return;
    }

    auto& buffer      = *bufferToFill.buffer;
    const int numSamples  = bufferToFill.numSamples;
    const int startSample = bufferToFill.startSample;

    // 뒤에서 outL/outR로 같은 버퍼 채널에 덮어쓰므로, 입력을 먼저 스크래치로 복사해 둔다.
    const int lastChannel   = buffer.getNumChannels() - 1;
    const int guitarChannel = juce::jlimit(0, lastChannel, guitarChannelIndex.load());
    const int vocalChannel  = juce::jlimit(0, lastChannel, vocalChannelIndex.load());
    const float* guitarRead = buffer.getReadPointer(guitarChannel, startSample);
    const float* vocalRead  = buffer.getReadPointer(vocalChannel,  startSample);
    std::copy(guitarRead, guitarRead + numSamples, guitarInputScratch.begin());
    std::copy(vocalRead,  vocalRead  + numSamples, vocalInputScratch.begin());

    float* outL = buffer.getWritePointer(0, startSample);
    float* outR = buffer.getWritePointer(1, startSample);

    mode2Controller.processBlock(guitarInputScratch.data(), vocalInputScratch.data(),
                                 outL, outR, numSamples);
}
```

### 2-2. 오디오 스레드 규칙 (플랫폼 무관)

- 메모리 할당·락 획득·파일 I/O 금지. 스크래치 배열은 `prepareToPlay`에서 미리 확보한다.
- 녹음은 JUCE `AudioFormatWriter::ThreadedWriter`(락 없는 FIFO)에 밀어넣기만 하고, 실제 디스크 쓰기는
  별도 `TimeSliceThread`가 한다.
- UI ↔ 오디오 스레드 사이의 값 전달은 전부 `std::atomic`이다(채널 인덱스, 게인, 미터 값 등).
- 피치 시프터 백엔드를 런타임에 교체할 때만 예외적으로 `deviceManager.getAudioCallbackLock()`을 잡는다.

---

## 3. 채널 매핑 — 하드코딩하지 않는다

장치마다 물리 배선이 다르므로 "ch0=기타"를 코드에 박아두지 않는다.

- 화면 콤보박스에서 "기타 채널 / 목소리 채널"을 선택 → `std::atomic<int> guitarChannelIndex{0}`,
  `vocalChannelIndex{1}` 에 저장 → XML로 영속화.
  - **Windows**: `%APPDATA%\VocalGuitarApp\Mode2ChannelMap.xml`
  - macOS: `~/Library/VocalGuitarApp/Mode2ChannelMap.xml`
- 읽을 때 `juce::jlimit(0, lastChannel, ...)`으로 클램프한다. 장치가 바뀌어 채널 수가 줄어도
  범위 밖 메모리를 읽지 않게 하기 위해서다.
- 배선이 의심스러우면 `-DMODE2_DIAGNOSTIC_LOG=1`로 빌드한다. **24블록마다 전체 입력 채널의 RMS**를
  로그로 찍어서 "채널 순서가 바뀐 것"과 "마이크가 기타 소리를 줍는 것(bleed)"을 구분할 수 있다.

```cpp
#if MODE2_DIAGNOSTIC_LOG
static int chLogCount = 0;
if (++chLogCount % 24 == 0)
{
    juce::String line = "[CH] ";
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        const float* read = buffer.getReadPointer(ch, startSample);
        double ss = 0.0;
        for (int i = 0; i < numSamples; ++i) ss += (double) read[i] * read[i];
        line += "ch" + juce::String(ch) + "=" + juce::String(std::sqrt(ss / numSamples), 5) + "  ";
    }
    juce::Logger::writeToLog(line);
}
#endif
```

---

## 4. 샘플레이트 의존성 (실제로 물렸던 함정 — Windows에서 더 심해질 수 있음)

분석 창 크기를 **샘플 수**로 정의하면, 샘플레이트가 절반이 될 때 창의 **시간 길이가 두 배**가 된다.
macOS에서 에어팟을 입력으로 쓰면 장치가 24kHz로 열려서 이 문제가 실제로 발생했다.
그래서 창 크기를 현재 샘플레이트에 비례해 다시 계산한다.

```cpp
constexpr int    guitarPitchWindowSize = 2048;   // 48kHz 기준 샘플 수
constexpr int    vocalPitchWindowSize  = 2048;
constexpr double pitchWindowReferenceSampleRate = 48000.0;

inline int scalePitchWindowForSampleRate(int referenceSamples, double sampleRate)
{
    const int scaled = (int) (referenceSamples * sampleRate / pitchWindowReferenceSampleRate + 0.5);
    return scaled < 256 ? 256 : scaled;
}
```

48kHz → 2048샘플, 24kHz → 1024샘플. 어느 쪽이든 **실제 창 길이는 약 42.7ms로 고정**된다.

> **Windows에서 더 나쁜 이유**: Windows에서 블루투스 헤드셋의 마이크를 쓰면 **HFP(핸즈프리) 프로파일**로
> 전환되어 **모노 8kHz(CVSD) 또는 16kHz(mSBC)** 까지 떨어진다. 장치 목록에도
> `헤드셋(Hands-Free AG Audio)` 과 `헤드폰(Stereo)` 두 개가 따로 뜨는데, 마이크를 쓰려고 앞의 것을
> 고르는 순간 **출력 음질까지 같이 무너진다.** 8kHz에서는 창이 341샘플까지 줄어 저음 목소리(100Hz)에
> 들어오는 주기 수가 3~4개밖에 안 되므로 피치 추정이 크게 불안정해진다.
> **결론: Windows에서는 블루투스 마이크를 쓰지 말고 유선 마이크 또는 오디오 인터페이스를 쓴다.**

---

## 5. 각 채널의 소비 경로 (플랫폼 무관 — 코드가 동일)

### 5-1. 기타 채널 (음정 컨트롤러 — 출력에 섞이지 않음)

`GuitarTargetTracker::processBlock(guitarIn, numSamples)` 한 덩어리로 처리:

1. **온셋 검출** — 블록 RMS 에너지 플럭스가 EMA 베이스라인의 **1.6배**를 넘고, 직전 온셋에서
   **50ms(refractory)** 이상 지났을 때만 온셋. 에너지 하한 `1.0e-4`.
2. **피치 검출** — MPM(McLeod Pitch Method, NSDF 자기상관). 탐색 범위 **70~1200Hz**
   (6번줄 개방현 E2 82Hz ~ 1번줄 12프렛 E5 659Hz를 덮음). RMS가 `0.002` 미만이면 결과를 무효화.
3. **반음 양자화 + 히스테리시스(±0.65반음)** — 검출된 "순간 주파수"가 아니라 "연주한 음표"로 변환.
4. **상태 기계 안정화** — `IDLE → ATTACK → COLLECT → STABLE → COAST`.
   출력은 `hasTarget`, `targetMidi`, `fadeGain`, `state`.
   - `attackSettleSeconds = 0.025` (피킹 직후 이전 음이 창에서 빠질 시간)
   - `stableToleranceSemitones = 3.0` (이 안의 변화는 재조준 없이 즉시 반영 — 스케일·벤딩 추종)
   - `coastHoldSeconds = 0.35`, `coastFadeSeconds = 0.25` (목표 소실 시 유지 후 감쇠)

### 5-2. 목소리 채널

`Mode2Controller::processBlock` 안의 실행 순서 그대로:

1. **기타 유입 상쇄(GuitarBleedCanceller)** — 목소리 마이크에 새어 들어온 기타를, 기타 채널을 참조
   신호로 삼아 **NLMS 적응 FIR(256탭, step 0.08)** 로 뺀다. **부스트 전**에 거는 이유: 부스트 뒤에 걸면
   사용자가 슬라이더를 움직일 때마다 학습한 경로 이득이 어긋나 다시 수렴해야 한다.
   **기본값은 꺼짐** — 기타 라인 입력 + 헤드폰 환경에서는 유입이 사실상 없고, 켜면 뺄 게 없는데도
   목소리를 깎아 명료도가 떨어졌다.
2. **입력 부스트** — 단순 스칼라 곱, 기본 `2.0`.
3. **피치 검출** — 같은 MPM이지만 탐색 범위가 **100~450Hz**로 좁다. 넓으면 배주기 피크(한 옥타브 아래)와
   배음 피크(한 옥타브 위)가 후보에 들어와 출력 음정이 옥타브 단위로 튄다. 신뢰도 게이트 **0.65**.
4. **3탭 미디언 필터** — 단발성 검출 이상치를 제거. 지속되는 옥타브 변화는 통과시킨다.
5. **딜레이 버퍼(제어 lookahead)** — 제어 경로와 오디오 경로의 정렬량. **현재 0ms**.
   측정 결과 이 검출기의 추정값은 창 중앙이 아니라 **창의 최신 끝**에 대응했고, 오디오를 늦출수록
   오차가 단조 증가했다(21.3ms에서 중앙값 17.3cents → 0ms에서 8.4cents).
6. **피치 시프트** — Rubber Band / SoundTouch / WORLD 중 런타임 선택
   (**어느 것이 실제로 쓰이는지는 플랫폼별로 다르다 → 9절 D-6**).
   보정량 = `(기타 목표 MIDI + 12 × 옥타브이동) − 내 음정`, `pitchFollowStrength = 1.0`(완전 일치).
   글라이드 기본 `250 반음/초`. WORLD 경로에는 반음 대신 **기타의 절대 F0(Hz)** 를 직접 준다.

### 5-3. 출력단

```cpp
const float outputGain = calibratedCeiling * outputVolume * feedbackDuckGain * outputTripGain;

std::copy(shiftedVocalScratch.begin(), shiftedVocalScratch.begin() + numSamples, outL);
notchSuppressor.processBlock(outL, numSamples);          // 하울링 주파수만 좁게 제거
noiseGate.processBlock(outL, numSamples, boostedRms);    // 판정은 부스트 후 목소리 RMS로

for (int i = 0; i < numSamples; ++i)
{
    const float sample = std::clamp(outL[i] * outputGain, -0.95f, +0.95f);  // 최종 리미터
    outL[i] = sample;
    outR[i] = sample;   // 좌우 동일한 모노
}
```

- 노이즈 게이트 임계 기본 `0.024` — **부스트를 적용한 뒤의 RMS**와 비교된다. 부스트를 바꾸면 이 값도
  같이 바꿔야 한다(한쪽만 바꾸면 게이트가 잡음에 열리거나 작은 목소리에 안 열린다).
- 하울링 억제(노치 + 트립 게이트)는 **기본 꺼짐**. 헤드폰 모니터링이면 되먹임 경로가 물리적으로 없고,
  켜면 정상 노래를 되먹임으로 오판해 음질이 떨어진다(깨끗함 중앙값 96.0% → 92.0%).
- **드라이(원본 목소리)를 병렬로 섞지 않는다.** 시프터 출력은 입력보다 늦어서 두 경로를 섞으면
  콤 필터링과 이중 음정이 생긴다. 보정을 끌 때도 경로는 하나로 유지하고 보정량만 0으로 내린다.

---

## 6. 진단 녹음 파일 형식 (플랫폼 무관)

같은 연주를 두 번 할 수 없으므로, 파라미터 A/B 비교를 하려면 입력까지 그대로 남겨야 한다.

| 항목 | 값 |
|---|---|
| 포맷 | WAV, **24비트 정수**, **3채널** |
| 샘플레이트 | 장치의 현재 샘플레이트 그대로 |
| ch0 | 기타 입력 (원본) |
| ch1 | 목소리 입력 (**부스트 전, 유입 상쇄 전**) |
| ch2 | 당시 최종 출력 |
| 경로 (Windows) | `%APPDATA%\VocalGuitarApp\recordings\mode2_YYYYMMDD_HHMMSS.wav` |
| 경로 (macOS) | `~/Library/VocalGuitarApp/recordings/mode2_YYYYMMDD_HHMMSS.wav` |

```cpp
juce::WavAudioFormat wav;
// ch0 = 기타 입력, ch1 = 목소리 입력(부스트 전), ch2 = 최종 출력.
auto* writer = wav.createWriterFor(stream.get(), currentSampleRate, 3, 24, {}, 0);
threadedWriter.reset(new juce::AudioFormatWriter::ThreadedWriter(writer, recorderThread, 65536));
```

이 파일을 오프라인 도구 `Mode2Offline`에 넣으면 **앱과 동일한 `Mode2Controller`에 동일한 블록 크기로**
다시 통과시킬 수 있다. 연주가 고정되므로 변수가 파라미터 하나만 남아 A/B 비교가 성립한다.

```
Mode2Offline <입력.wav> <출력.wav> [옵션]
  --glide=250       글라이드 속도(반음/초)
  --boost=2.0       목소리 입력 부스트
  --volume=1.0      최종 출력 볼륨
  --octave=-1       목표 옥타브 이동
  --gate=0.024      노이즈 게이트 임계(부스트 후 RMS). --gate=off 로 끄기
  --howlguard=on|off  하울링 억제(오프라인 기본 off)
  --bleed=on|off    기타 유입 상쇄
  --trip=0.35       출력 트립 임계
  --block=512       블록 크기
  --lookahead=0     제어/오디오 경로 정렬량(ms)
  --shifter=rubberband|soundtouch|world
```

---

## 7. Windows 빌드 방법

**전제**: Visual Studio 2022 (Desktop development with C++ 워크로드), CMake 3.22+, Git.
JUCE는 CMake가 자동으로 내려받으므로 따로 설치할 필요 없다.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**산출물 경로** (MSVC는 멀티 컨피그 제너레이터라 `Release\` 하위 폴더가 하나 더 생긴다):

```
build\VocalGuitarApp_artefacts\Release\Vocal Guitar App.exe
build\Mode2Offline_artefacts\Release\Mode2Offline.exe
build\Mode2Tests_artefacts\Release\Mode2Tests.exe
```

macOS(단일 컨피그 Makefile 제너레이터) 기준 경로는 `build/Mode2Offline_artefacts/Debug/Mode2Offline`
형태로, **구성 폴더 이름과 `.exe` 확장자 유무가 다르다.**

### 7-1. 선택: ASIO 켜기 (저지연이 필요하면)

JUCE는 라이선스 문제로 ASIO SDK를 동봉하지 않고, 현재 `CMakeLists.txt`에는 `JUCE_ASIO` 정의가 없다.
즉 **지금 상태로 Windows 빌드하면 ASIO는 목록에 아예 안 나온다.** 쓰려면 Steinberg에서 ASIO SDK를
내려받고 다음을 추가한다:

```cmake
target_compile_definitions(VocalGuitarApp PUBLIC JUCE_ASIO=1)
target_include_directories(VocalGuitarApp PRIVATE "C:/path/to/asiosdk/common")
```

ASIO 없이 저지연이 필요하면 **WASAPI 독점 모드(Exclusive)** 를 고른다.

### 7-2. 선택: Rubber Band 붙이기

`CMakeLists.txt`는 Rubber Band를 시스템 경로에서 찾는다(`find_path` / `find_library`).
Windows에는 Homebrew/apt가 없으므로 **기본적으로 못 찾고 `HAVE_RUBBERBAND=0`으로 빌드된다**(9절 D-6).
붙이려면 vcpkg를 쓰는 게 가장 간단하다:

```
vcpkg install rubberband:x64-windows
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

CMake 설정 단계 로그에 `Rubber Band found: ...` 가 찍히면 성공, `Rubber Band not found` 면 실패다.

---

## 8. Windows 환경 설정 체크리스트 (소리가 안 들어올 때)

1. **마이크 권한** — 설정 → 개인 정보 및 보안 → 마이크 →
   **"앱이 마이크에 액세스하도록 허용"** 과 **"데스크톱 앱이 마이크에 액세스하도록 허용"** 을 모두 켠다.
   ⚠ 차단되어 있으면 **오류 없이 무음만 들어온다.** macOS처럼 권한 요청 팝업이 뜨지 않으므로
   증상만 보고는 원인을 알기 어렵다.
2. **드라이버 종류** — 앱의 오디오 설정에서 `Windows Audio (Exclusive Mode)` 또는 ASIO를 고른다.
   `DirectSound`는 지연이 가장 크므로 피한다.
3. **샘플레이트 고정** — 제어판 `mmsys.cpl` → 녹음/재생 장치 → 속성 → 고급 → 기본 형식을
   **입력과 출력 모두 48000Hz**로 맞춘다. 공유 모드에서는 이 값이 그대로 앱에 내려온다.
4. **마이크 신호 처리 끄기** — 같은 속성 창의 "고급/향상 기능" 탭에서 **오디오 향상(Audio Enhancements)**,
   AGC, 노이즈 억제를 전부 끈다. 이런 처리가 켜져 있으면 피치 분석이 망가진다.
5. **통신 자동 볼륨 조절 끄기** — 사운드 설정 → 통신 → **"아무 작업도 하지 않음"**.
   기본값은 통화 감지 시 다른 소리를 80% 줄이는 것이라, 출력이 제멋대로 작아진다.
6. **블루투스 마이크 금지** — 4절 참고. HFP로 떨어져 8/16kHz 모노가 된다.
7. **입출력 장치를 같은 하드웨어로** — 다른 물리 장치를 섞으면 클럭 드리프트로 끊김이 생긴다(9절 D-4).

---

## 9. Windows ↔ macOS 차이 정리

| # | 항목 | Windows | macOS | 근거 |
|---|---|---|---|---|
| D-1 | 드라이버 계층 | WASAPI(공유/독점), DirectSound, ASIO(선택 빌드) | CoreAudio 단일 | 플랫폼 일반 |
| D-2 | 샘플레이트 결정권 | 공유 모드는 **제어판 설정값 고정**. 독점/ASIO만 앱이 요청 가능 | 앱·Audio MIDI 설정에서 자유롭게 변경 | 플랫폼 일반 |
| D-3 | 장치 합치기 | **OS 기능 없음.** ASIO4ALL이나 Voicemeeter 같은 도구 필요 | **Aggregate Device** 기본 제공 | 플랫폼 일반 |
| D-4 | 입출력 장치 분리 | WASAPI/DirectSound는 입력·출력을 따로 고를 수 있으나 **클럭이 달라 드리프트 발생**. ASIO는 단일 장치 | Aggregate Device가 드리프트 보정을 해줌 | 플랫폼 일반 |
| D-5 | 설정·녹음 저장 위치 | `%APPDATA%\VocalGuitarApp\` | `~/Library/VocalGuitarApp/` | 코드 확인됨 (`File::userApplicationDataDirectory`) |
| D-6 | **기본 피치 시프터** | Rubber Band를 못 찾으므로 앱 기본은 **SoundTouch** | Homebrew로 설치되어 **Rubber Band** | **코드 확인됨** (아래 상세) |
| D-7 | ASIO 사용 가능 여부 | `JUCE_ASIO` 정의가 없어 **현재 꺼짐**. SDK + 정의 추가 필요 | 해당 없음 | 코드 확인됨 (CMakeLists에 JUCE_ASIO 없음) |
| D-8 | 마이크 권한 | 설정에서 수동 허용. 차단 시 **무음, 오류 없음** | 실행 시 권한 팝업(`MICROPHONE_PERMISSION_ENABLED TRUE`가 Info.plist 생성) | 코드 확인됨 |
| D-9 | OS의 마이크 전처리 | Audio Enhancements / AGC / 노이즈 억제가 기본 켜져 있는 드라이버 많음 | 기본적으로 없음(에어팟 자체 처리는 별개) | 플랫폼 일반 |
| D-10 | 블루투스 마이크 | HFP로 전환 → **모노 8kHz(CVSD) / 16kHz(mSBC)**, 출력 음질도 같이 하락 | 24kHz 수준 유지 | 플랫폼 일반 |
| D-11 | 빌드 산출물 경로 | `build\<타깃>_artefacts\Release\*.exe` (구성 폴더 있음) | `build/<타깃>_artefacts/Debug/*` | 코드 확인됨 (제너레이터 차이) |
| D-12 | 콜백 데이터 형식 | **동일** (float32, 비인터리브, −1~+1) | **동일** | 코드 확인됨 |
| D-13 | DSP 파이프라인·파라미터 | **동일** | **동일** | 코드 확인됨 |

### D-6 상세: 시프터 백엔드가 조용히 바뀐다 (가장 중요한 실질적 차이)

`CMakeLists.txt`는 Rubber Band를 시스템 경로에서 찾는다:

```cmake
find_path(RUBBERBAND_INCLUDE_DIR NAMES rubberband/RubberBandStretcher.h)
find_library(RUBBERBAND_LIBRARY  NAMES rubberband)
# 찾으면 HAVE_RUBBERBAND=1, 못 찾으면 0
```

- **WORLD는 vendored 소스(`external/world`)라 항상 빌드된다** → `HAVE_WORLD=1` 항상 참
- **SoundTouch도 vendored(`external/soundtouch`)라 항상 빌드된다** → `HAVE_SOUNDTOUCH=1` 항상 참
- **Rubber Band만 시스템 설치에 의존한다** → Windows에서는 보통 `HAVE_RUBBERBAND=0`

그 결과 두 곳에서 동작이 갈린다.

**(1) 앱의 기본 백엔드** — 컴파일 타임에 결정된다:

```cpp
#if defined(HAVE_RUBBERBAND)
    Backend activeBackend = Backend::RubberBand;   // macOS
#elif defined(HAVE_SOUNDTOUCH)
    Backend activeBackend = Backend::SoundTouch;   // Windows (기본)
#else
    Backend activeBackend = Backend::World;
#endif
```

**(2) 없는 백엔드를 요청했을 때의 폴백 순서** — 요청 → World → RubberBand → SoundTouch:

```cpp
void PitchShifterEngine::setBackend(Backend requested)
{
    if (isBackendAvailable(requested))            activeBackend = requested;
    else if (isBackendAvailable(Backend::World))  activeBackend = Backend::World;   // ← 여기로 온다
    else if (isBackendAvailable(Backend::RubberBand)) activeBackend = Backend::RubberBand;
    else                                          activeBackend = Backend::SoundTouch;
}
```

**실무적 결과 3가지:**

1. Windows에서 `Mode2Offline --shifter=rubberband`(오프라인 도구의 **기본값**)를 실행하면
   경고 없이 **WORLD로 처리된다.** mac에서 낸 결과와 직접 비교하면 안 된다.
   비교하려면 양쪽 모두 `--shifter=soundtouch` 처럼 명시적으로 같은 백엔드를 지정해야 한다.
2. 앱을 그냥 실행하면 mac은 Rubber Band, Windows는 SoundTouch로 소리가 난다. **음색이 다르게 들린다.**
3. SoundTouch 경로에서만 **고역 보강(timbre presence compensation)** 이 추가로 걸린다.
   Rubber Band는 포먼트를 직접 보존하므로 이 보정을 적용하지 않기 때문이다:

```cpp
const bool needsPresenceCompensation =
    pitchShifter.getActiveBackend() == PitchShifterEngine::Backend::SoundTouch;
```

또한 파라미터 파일에 적힌 측정 수치(예: lookahead 스윕의 cents 값)는 **Rubber Band 기준으로 측정된 것**이므로,
SoundTouch/WORLD에서는 그대로 성립하지 않을 수 있다.

---

## 10. 요약 (사실 목록)

1. 인터페이스 입력은 **채널별로 분리된 float32 배열**, 값 범위 −1.0~+1.0, 길이는 장치 블록 크기.
   **이 형식은 Windows와 macOS가 완전히 같다.**
2. JUCE `getNextAudioBlock`에서 **입력 버퍼와 출력 버퍼는 같은 메모리**다. 읽기 전에 지우면 안 되고,
   출력을 쓰기 전에 입력을 스크래치로 복사해야 한다.
3. 채널 번호는 장치마다 다르므로 하드코딩하지 않고, UI 선택 → atomic 전달 → XML 저장한다.
4. 샘플레이트는 장치가 정한다. **샘플 수로 정의된 분석 창은 반드시 샘플레이트에 비례해 스케일**해야 한다.
   Windows에서 블루투스 마이크를 쓰면 8/16kHz까지 떨어지므로 유선을 쓴다.
5. 기타 채널에서는 음정 정보만 추출하고, 스피커로는 변조된 목소리 하나만 나간다.
6. 두 신호의 피치 검출기는 같은 MPM이지만 **탐색 범위가 다르다**(기타 70~1200Hz, 목소리 100~450Hz).
7. **Windows에서는 Rubber Band가 빠져 기본 시프터가 SoundTouch가 되고, `--shifter=rubberband` 요청은
   WORLD로 조용히 폴백한다.** mac과 소리·측정치를 비교하려면 백엔드를 명시적으로 맞춰야 한다.
8. Windows 고유 함정: 마이크 권한 차단 시 무음(오류 없음), 공유 모드의 샘플레이트 고정,
   드라이버의 마이크 전처리, 통신 자동 볼륨 조절, Aggregate Device 부재.
9. 디버깅 표준 절차는 3채널 WAV 녹음 → `Mode2Offline` 오프라인 재현이다.
