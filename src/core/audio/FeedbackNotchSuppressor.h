#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <vector>

// 적응형 하울링 노치 억제기.
//
// 하울링은 특정 주파수에서 좁게 링잉하는 현상이다. 전체 게인을 눌러 소리를 답답하게 만드는
// 대신, 스펙트럼에서 "좁은 대역에 지속적으로 몰리는 성분"을 찾아 그 주파수에만 좁은 노치를
// 걸어 고리 이득을 떨어뜨린다. 음색 손상이 거의 없어 게인 다운보다 훨씬 유리하다.
//
// 분석은 노치를 걸기 전(pre-filter) 신호로 한다. 노치가 하울링을 실제로 죽이면 마이크로
// 되돌아오는 성분도 사라져 스펙트럼이 식으므로, 유지 시간이 지나면 노치를 자동 해제한다.
class FeedbackNotchSuppressor
{
public:
    static constexpr int maxNotches = 4;

    void prepare(double sampleRateIn, int maxBlockSize);
    void reset();

    void setEnabled(bool shouldEnable) { enabled = shouldEnable; }

    // 모노 블록을 제자리에서 처리한다.
    void processBlock(float* samples, int numSamples);

    int getActiveNotchCount() const;
    // index번째 활성 노치의 주파수(Hz). 없으면 0.
    float getActiveNotchFrequency(int index) const;

private:
    static constexpr int fftOrder = 10;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int spectrumBins = fftSize / 2;

    // 하울링이 실제로 일어나는 대역만 본다. 저역은 목소리 기본주파수·럼블과 섞이므로 제외.
    static constexpr float searchMinHz = 180.0f;
    static constexpr float searchMaxHz = 8000.0f;

    // 좁은 대역이 주변 평균보다 이만큼 높으면 하울링 후보로 본다.
    static constexpr float peakToFloorRatio = 8.0f;
    // 같은 주파수가 이만큼의 연속 분석 프레임에서 후보로 잡히면 노치를 건다.
    static constexpr int framesToConfirm = 3;

    static constexpr float notchQ = 18.0f;
    static constexpr float notchGainDb = -18.0f;
    // 노치를 무조건 유지하는 시간. 짧으면 걸었다 풀었다를 반복(채터링)한다.
    static constexpr double notchHoldSeconds = 6.0;

    // 럼블 제거용 하이패스. 하울링 고리에 기여하는 저역 에너지를 없애면서 음색 손상은 없다.
    static constexpr float highPassHz = 80.0f;

    struct Notch
    {
        bool active = false;
        float frequencyHz = 0.0f;
        int holdBlocksRemaining = 0;
        juce::dsp::IIR::Filter<float> filter;
    };

    double sampleRate = 44100.0;
    bool enabled = true;

    juce::dsp::FFT fft { fftOrder };
    std::vector<float> fftScratch;      // 2 * fftSize (juce FFT 요구)
    std::vector<float> analysisWindow;  // 최근 fftSize 샘플
    std::vector<float> hannWindow;
    std::vector<float> sortedMagnitudes;
    int analysisFilled = 0;

    std::array<Notch, maxNotches> notches;
    int candidateBin = -1;
    int candidateFrameCount = 0;

    juce::dsp::IIR::Filter<float> highPass;

    void pushForAnalysis(const float* samples, int numSamples);
    void analyseSpectrum();
    void addNotch(float frequencyHz);
    bool hasNearbyNotch(float frequencyHz) const;
    void tickHold(int numSamples);
};
