#pragma once

#include <vector>

// 출력이 "기타 목표 음정에 실제로 맞았는가"를 재는 지표.
//
// 기존 오프라인 지표는 전부 입력·제어 측이다(보정량 궤적, wet 분포, 유입 상쇄량). 그 값들은
// 파이프라인이 무엇을 의도했는지는 알려주지만 결과가 그대로 나왔는지는 알려주지 않는다.
// 시프터가 큰 하향 시프트에서 무너지거나 백엔드 지연 보상이 어긋나도 입력 측 지표는 그대로다.
// 그래서 만들어진 출력을 다시 F0 분석해 목표와 cents 단위로 비교한다. 이 숫자가 있어야
// 백엔드 A/B와 파라미터 변경을 귀가 아니라 지표로 판정할 수 있다.
class PitchAccuracyMetrics
{
public:
    struct Result
    {
        bool valid = false;

        int totalFrames = 0;      // 분석 프레임 전체
        int voicedFrames = 0;     // 출력이 유성으로 판정된 프레임
        int comparedFrames = 0;   // 유성이면서 목표도 있던 프레임 = 아래 지표의 표본 수

        // 주 지표. 반음의 절반을 넘으면 사람 귀에 "다른 음"으로 들리기 시작한다.
        float withinFiftyCentsPercent = 0.0f;
        // 화음으로 겹쳐도 어색하지 않은 수준.
        float withinTwentyCentsPercent = 0.0f;

        float medianAbsCents = 0.0f;
        float p90AbsCents = 0.0f;
        // 부호를 살린 중앙값. 0에서 멀면 전체가 일정하게 높거나 낮게 나가는 계통 오차다
        // (지연 보상 오차나 옥타브 설정 실수와 달리 파라미터로 교정할 수 있다).
        float medianSignedCents = 0.0f;
        // 반음 6개를 넘는 오차는 사실상 옥타브/음이름을 잘못 잡은 경우다.
        float octaveErrorPercent = 0.0f;

        // 실측 정렬 지연. 이론값과 크게 다르면 지연 보상 자체가 틀렸다는 신호다.
        int bestLagSamples = 0;
        int nominalLagSamples = 0;
        float medianAbsCentsAtNominalLag = 0.0f;

        // 목표가 오래 같은 음에 머무르면 지연을 어긋내도 비교 결과가 거의 같아서, 실측 지연은
        // 넓은 구간 어디에나 놓일 수 있다(= 식별 불가). 그 상태의 숫자를 실측치라고 내놓으면
        // 멀쩡한 지연 보상을 틀렸다고 오판하게 된다. 아래 두 값으로 그 상황을 구분한다.
        bool lagIdentifiable = false;
        int lagPlateauSamples = 0;   // 사실상 동점인 지연 후보들이 퍼져 있는 폭
    };

    // output               : 측정할 출력 신호(모노)
    // absoluteTargetF0Trace: 입력 시간축 기준으로 샘플마다 그 순간 컨트롤러가 노린 절대 목표
    //                        F0(Hz). 목표가 없던 구간은 0.
    // nominalLagSamples    : 파이프라인이 알려준 이론 지연(제어 lookahead + 시프터 고유 지연).
    //                        탐색의 출발점으로만 쓴다.
    // searchRadiusSamples  : 이론값 주변으로 실제 지연을 탐색할 반경.
    static Result measure(const float* output,
                          int numSamples,
                          double sampleRate,
                          const std::vector<float>& absoluteTargetF0Trace,
                          int nominalLagSamples,
                          int searchRadiusSamples);
};
