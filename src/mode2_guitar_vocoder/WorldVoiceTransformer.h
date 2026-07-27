#pragma once

#include <vector>

// 전체 녹음을 WORLD로 분석한 뒤 F0만 교체하는 오프라인 품질 기준선.
// 실시간 콜백용이 아니다: Harvest/CheapTrick/D4C 분석은 전체 신호와 큰 작업 버퍼를 쓴다.
class WorldVoiceTransformer
{
public:
    struct Result
    {
        std::vector<float> audio;
        int voicedFrames = 0;
        int totalFrames = 0;
        int fftSize = 0;
    };

    static Result transform(const float* vocal,
                            int numSamples,
                            double sampleRate,
                            const std::vector<float>& shiftSemitones,
                            const std::vector<float>& absoluteTargetF0Hz,
                            float inputGain,
                            float outputGain,
                            bool gateEnabled,
                            float gateThreshold);
};
