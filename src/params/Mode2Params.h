#pragma once

namespace mode2::params
{
    // OnsetDetector
    constexpr float onsetMinIntervalSeconds = 0.08f;
    constexpr float onsetAdaptiveThresholdMultiplier = 1.6f;
    constexpr float onsetEnergyFloor = 1.0e-4f;

    // PitchStabilizer
    constexpr float attackSettleSeconds = 0.03f;
    constexpr float collectConvergenceToleranceSemitones = 0.3f;
    constexpr int collectRequiredStableBlocks = 3;
    constexpr float stableToleranceSemitones = 0.6f;
    constexpr float coastHoldSeconds = 0.35f;
    constexpr float coastFadeSeconds = 0.25f;

    // VocalDelayBuffer (5절 장치 ①) — "온셋 검출까지 걸리는 시간만큼만" 늦춘다.
    // OnsetDetector는 블록 단위로 판정하므로 검출 지연은 분석 블록 1개 분량이다.
    // 안정화 대기(attackSettleSeconds)까지 더하면 사양서가 경고한 대로 항상 지연 비용을
    // 치르게 되고, 되먹임 고리의 지연이 길어져 하울링이 걸릴 수 있는 후보 주파수가
    // 촘촘해진다(간격 = 1/지연 Hz). 노치로 잡기 어려워지므로 짧게 유지하는 편이 유리하다.
    constexpr int vocalArtificialDelayBlocks = 1;

    // CorrectionCalculator
    constexpr float pitchFollowStrength = 0.95f;

    // 목표 음을 부르는 사람의 옥타브로 접어서 보정한다. 기타를 치는 음역과 노래하는 음역이
    // 옥타브 단위로 다른 게 자연스러운 연주 방식이라, 사양서 4절처럼 12반음 차이를 그냥
    // 포기하면 보정이 아예 걸리지 않는다. 접으면 남는 차이는 항상 ±6반음 이내이고,
    // 피치 검출의 옥타브 오류도 0에 가깝게 접혀 자동으로 무해해진다.
    constexpr bool foldCorrectionToNearestOctave = true;

    // 옥타브를 접은 뒤 남은 차이에 적용되는 상한. 접기를 켜면 잔차가 최대 6반음이므로
    // 7에서는 발동하지 않는다. 값을 낮추면 "살짝 어긋난 경우만 보정"하게 만들 수 있다.
    constexpr float maxCorrectionSemitones = 7.0f;

    // PitchShifterEngine
    constexpr float shiftRampSeconds = 0.05f;

    // 신뢰도 게이트 — 이 아래면 드라이로 감쇠(M2-4)
    constexpr float vocalConfidenceGateThreshold = 0.5f;

    // 노이즈 게이트: 말하지 않는 구간의 실내 잡음·기타 유입·하울링 성장을 끊는다.
    // 판정은 부스트 적용 후 목소리 RMS로 한다(화면의 "목소리 입력 레벨"과 같은 기준).
    // 하울링을 더 세게 억제하기 위해 기본 임계를 높이고, 말이 끝나면 빠르게 닫히도록
    // hold/release를 짧게 잡는다(말꼬리가 짧게 잘릴 수 있으면 슬라이더로 낮추면 된다).
    constexpr float vocalNoiseGateThreshold = 0.045f;
    // 닫는 임계는 여는 임계보다 낮게 둬서 임계 근처 떨림을 막는다.
    constexpr float noiseGateHysteresisRatio = 0.6f;
    // 말 시작을 놓치지 않게 어택은 빠르게, 되먹임이 자랄 틈을 주지 않게 릴리스는 짧게.
    constexpr float noiseGateAttackSeconds = 0.005f;
    constexpr float noiseGateReleaseSeconds = 0.06f;
    // 짧은 무음(자음 사이 등)에 닫히지 않도록 유지 시간을 둔다.
    constexpr float noiseGateHoldSeconds = 0.05f;

    // 레벨 미터의 100% 기준 RMS. 게이트 임계 슬라이더도 같은 눈금을 쓴다.
    constexpr float levelMeterReferenceRms = 0.3f;

    // 하울링 자동 억제(7절 리스크 대응): 출력 레벨이 지속적으로 높게 유지되면 되먹임으로
    // 보고 게인을 빠르게 줄이고, 잠잠해지면 천천히 회복한다.
    constexpr float feedbackDuckOutputThreshold = 0.25f;
    constexpr float feedbackDuckAttackPerBlock = 0.82f;
    constexpr float feedbackDuckRecoveryPerBlock = 0.004f;
    constexpr float feedbackDuckMinimumGain = 0.08f;
    constexpr float outputLevelEmaCoeff = 0.2f;

    // 출력 리미터: 어떤 경우에도 폭주하지 않도록 마지막에 걸어두는 상한.
    constexpr float outputLimitPeak = 0.95f;

    // 출력 하울링 트립 게이트: feedbackDuck*은 천천히 눌러 내리는 방식이라 되먹임이 이미
    // 자란 뒤에는 반응이 느리다. 이건 출력이 기준을 넘는 순간 그 블록을 즉시 완전 묵음
    // 시키는 훨씬 강한 마지막 수단이다. 판정은 직전 블록의 출력 RMS로 한다(한 블록 지연,
    // feedbackDuck과 같은 구조).
    constexpr float outputTripThreshold = 0.35f;
    // 한번 트립되면 이 시간 동안은 레벨이 내려가도 무조건 묵음을 유지한다(바로 풀리면
    // 되먹임이 곧장 다시 차오르며 껌뻑거리게 된다).
    constexpr float outputTripHoldSeconds = 0.4f;
    // 유지 시간이 끝난 뒤 게인을 1.0까지 되돌리는 속도(블록당 증가량). 급하게 열면 다시
    // 트립되기 쉬우므로 서서히 되돌린다.
    constexpr float outputTripRecoveryPerBlock = 0.01f;
}
